#include "orderbook.h"
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>

namespace {
// Utility for logging with timestamp and thread id
std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt;
    localtime_r(&timer, &bt);
    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void logMessage(const std::string& message) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << "[" << getTimestamp() << "] [" << std::this_thread::get_id() << "] " << message << std::endl;
}
}

void printOrderDetails(const OrderBook::Order* order) {
    if (!order) return;
    std::cout << "Order ID: " << order->idNumber << "\n"
              << "Type: " << (order->buyOrSell ? "Buy" : "Sell") << "\n"
              << "Shares: " << order->shares << "\n"
              << "Price: " << order->limit << "\n"
              << "Entry Time: " << order->entryTime << "\n"
              << "Event Time: " << order->eventTime << "\n"
              << "Active: " << order->active.load() << "\n\n";
}

void printLimitDetails(const OrderBook::Limit* limit) {
    if (!limit) return;
    std::cout << "Limit Price: " << limit->limitPrice << "\n"
              << "Total Size: " << limit->size.load() << "\n"
              << "Order Count: " << limit->orderCount.load() << "\n\n";
    
    std::cout << "Orders at this limit:\n";
    OrderBook::Order* currentOrder = limit->headOrder;
    while (currentOrder) {
        printOrderDetails(currentOrder);
        currentOrder = currentOrder->next;
    }
}

void runThreadedTest() {
    // Create order book and start processing
    OrderBook::OrderBook book;
    book.start();
    
    // Register execution callback
    std::atomic<int> matchCount{0};
    book.registerExecutionCallback([&matchCount](const OrderBook::ExecutionReport& report) {
        logMessage("MATCH: Buy #" + std::to_string(report.buyOrderId) + 
                   " Sell #" + std::to_string(report.sellOrderId) + 
                   " Price: " + std::to_string(report.price) + 
                   " Qty: " + std::to_string(report.quantity));
        matchCount.fetch_add(1);
    });
    
    // Parameters for the test
    const int numThreads = 4;
    const int ordersPerThread = 50; // Reduced from 50 to keep output manageable
    const int maxPrice = 100;
    const int maxShares = 100;
    
    // Create producer threads - each will submit orders
    std::vector<std::thread> producers;
    
    // Function to generate random orders
    auto producerFunc = [&book, maxPrice, maxShares, ordersPerThread](int threadId) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> priceDist(1, maxPrice);
        std::uniform_int_distribution<> sharesDist(1, maxShares);
        std::uniform_int_distribution<> typeDist(0, 1);
        
        for (int i = 0; i < ordersPerThread; ++i) {
            bool isBuy = typeDist(gen) == 1;
            int shares = sharesDist(gen);
            int price = priceDist(gen);
            
            int orderId = book.addOrder(isBuy, shares, price);
            // Only log every 5th order to reduce output
            if (i % 5 == 0) {
                logMessage("Thread " + std::to_string(threadId) + 
                        " added " + (isBuy ? "BUY" : "SELL") + 
                        " order #" + std::to_string(orderId) + 
                        " for " + std::to_string(shares) + 
                        " shares at $" + std::to_string(price));
            }
            
            // Random delay between orders
            std::this_thread::sleep_for(std::chrono::milliseconds(10 + typeDist(gen) * 10));
        }
        logMessage("Thread " + std::to_string(threadId) + " finished adding orders");
    };
    
    // Start producer threads
    logMessage("Starting producer threads...");
    for (int i = 0; i < numThreads; ++i) {
        producers.emplace_back(producerFunc, i);
    }
    
    // Wait for all producers to finish
    for (auto& thread : producers) {
        thread.join();
    }
    
    // Give some time for matching to complete
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Print statistics
    logMessage("All producer threads finished");
    logMessage("Total orders processed: " + std::to_string(numThreads * ordersPerThread));
    logMessage("Total matches executed: " + std::to_string(matchCount.load()));
    
    // Print current book state
    logMessage("Best bid: $" + std::to_string(book.getBestBid()));
    logMessage("Best ask: $" + std::to_string(book.getBestAsk()));
    
    // Show volumes at some price points
    for (int price = 10; price <= 90; price += 10) {
        int volume = book.getVolumeAtPrice(price);
        if (volume > 0) {
            logMessage("Volume at $" + std::to_string(price) + ": " + std::to_string(volume) + " shares");
        }
    }
    
    // Cleanup
    book.stop();
    logMessage("Order book stopped");
}

int main() {
    std::cout << "Starting multi-threaded OrderBook test...\n\n";
    runThreadedTest();
    return 0;
}