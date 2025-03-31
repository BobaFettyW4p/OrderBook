#pragma once
#include <memory>
#include <unordered_map>
#include <iostream>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <optional>
#include <functional>
#include <list>

namespace OrderBook {

// Forward declarations
class Order;
class Limit;

// Thread-safe queue for order operations
template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};

public:
    ThreadSafeQueue() = default;
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(std::move(value));
        cv.notify_one();
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) {
            return false;
        }
        value = std::move(queue.front());
        queue.pop();
        return true;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue.front());
        queue.pop();
        return value;
    }

    void wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return !queue.empty() || done.load(); });
        if (done.load() && queue.empty()) {
            return;
        }
        value = std::move(queue.front());
        queue.pop();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }

    void shutdown() {
        done.store(true);
        cv.notify_all();
    }

    bool is_done() const {
        return done.load();
    }
};

// Order operation types
enum class OrderOperation {
    Add,
    Cancel,
    Modify,
    Match // Process matching
};

// Operation request structure
struct OrderRequest {
    OrderOperation operation;
    int orderId;
    bool isBuy;
    int shares;
    int price;
    int entryTime;
};

// Execution report for matched orders
struct ExecutionReport {
    int buyOrderId;
    int sellOrderId;
    int price;
    int quantity;
    int timestamp;
};

// Memory-safe Order class
class Order {
public:
    int idNumber;
    bool buyOrSell;  // true for buy, false for sell
    int shares;
    int limit;
    int entryTime;
    int eventTime;
    std::atomic<bool> active{true}; // Atomic flag for order status
    
    // Smart pointers for linked list
    std::shared_ptr<Order> next;
    std::weak_ptr<Order> prev;
    std::weak_ptr<Limit> parentLimit;
    
    Order() = default;
    Order(int id, bool isBuy, int qty, int price, int time) 
        : idNumber(id), buyOrSell(isBuy), shares(qty), limit(price), 
          entryTime(time), eventTime(time), active(true) {}
};

// Memory-safe Limit class
class Limit {
public:
    int limitPrice;
    std::atomic<int> size{0};
    std::atomic<int> orderCount{0};
    std::mutex limitMutex; // We need a mutex per limit level for linked list operations
    
    // Smart pointers for binary tree structure
    std::weak_ptr<Limit> parent;
    std::shared_ptr<Limit> leftChild;
    std::shared_ptr<Limit> rightChild;
    
    // Linked list of orders at this limit
    std::shared_ptr<Order> headOrder;
    std::shared_ptr<Order> tailOrder;
    
    Limit() = default;
    explicit Limit(int price) : limitPrice(price) {}
};

class OrderBook {
private:
    std::shared_ptr<Limit> buyTree;
    std::shared_ptr<Limit> sellTree;
    std::unordered_map<int, std::shared_ptr<Order>> orderMap;
    std::unordered_map<int, std::shared_ptr<Limit>> limitMap;
    
    // Thread synchronization primitives
    mutable std::mutex bookMutex; // Protects tree structure and maps
    ThreadSafeQueue<OrderRequest> orderQueue; // Queue for order operations
    ThreadSafeQueue<ExecutionReport> executionReports; // Queue for execution reports
    std::atomic<bool> running{false};
    std::thread matchingThread;
    std::atomic<int> nextOrderId{1};
    
    // Helper methods for tree operations
    std::shared_ptr<Limit> insertLimit(std::shared_ptr<Limit> root, std::shared_ptr<Limit> newLimit, bool isBuy) {
        if (!root) {
            return newLimit;
        }
        
        if ((isBuy && newLimit->limitPrice > root->limitPrice) ||
            (!isBuy && newLimit->limitPrice < root->limitPrice)) {
            root->rightChild = insertLimit(root->rightChild, newLimit, isBuy);
            if (root->rightChild) {
                root->rightChild->parent = root;
            }
        } else {
            root->leftChild = insertLimit(root->leftChild, newLimit, isBuy);
            if (root->leftChild) {
                root->leftChild->parent = root;
            }
        }
        
        return root;
    }
    
    std::shared_ptr<Limit> findLimit(std::shared_ptr<Limit> root, int price) const {
        if (!root) return nullptr;
        
        if (root->limitPrice == price) {
            return root;
        } else if ((root->limitPrice < price && root->rightChild) ||
                  (root->limitPrice > price && !root->leftChild)) {
            return findLimit(root->rightChild, price);
        } else {
            return findLimit(root->leftChild, price);
        }
    }
    
    void addOrder(std::shared_ptr<Order> order, std::shared_ptr<Limit> limit) {
        // Lock the specific limit level
        std::lock_guard<std::mutex> lock(limit->limitMutex);
        
        // Update order's parent limit
        order->parentLimit = limit;
        
        // Add to end of limit's order list
        if (!limit->headOrder) {
            limit->headOrder = order;
            limit->tailOrder = order;
        } else {
            order->prev = limit->tailOrder;
            if (limit->tailOrder) {
                limit->tailOrder->next = order;
            }
            limit->tailOrder = order;
        }
        
        // Update limit size and count atomically
        limit->size.fetch_add(order->shares);
        limit->orderCount.fetch_add(1);
    }
    
    void processOrderQueue() {
        while (running.load()) {
            // Process incoming order requests
            OrderRequest request;
            orderQueue.wait_and_pop(request);
            
            if (!running.load()) {
                break;
            }
            
            switch (request.operation) {
                case OrderOperation::Add:
                    processAddOrder(request.orderId, request.isBuy, request.shares, request.price, request.entryTime);
                    break;
                case OrderOperation::Cancel:
                    processCancelOrder(request.orderId);
                    break;
                case OrderOperation::Modify:
                    processModifyOrder(request.orderId, request.shares);
                    break;
                case OrderOperation::Match:
                    processMatching();
                    break;
            }
        }
    }
    
    void processAddOrder(int orderId, bool isBuy, int shares, int price, int entryTime) {
        std::lock_guard<std::mutex> lock(bookMutex);
        
        // Create new order using make_shared
        auto order = std::make_shared<Order>(orderId, isBuy, shares, price, entryTime);
        
        // Find or create limit price level
        std::shared_ptr<Limit> limit;
        auto it = limitMap.find(price);
        if (it == limitMap.end()) {
            // Create new limit level
            auto newLimit = std::make_shared<Limit>(price);
            limit = newLimit;
            
            // Insert into appropriate tree
            if (isBuy) {
                buyTree = insertLimit(buyTree, limit, true);
            } else {
                sellTree = insertLimit(sellTree, limit, false);
            }
            
            limitMap[price] = limit;
        } else {
            limit = it->second;
        }
        
        // Add order to limit level
        addOrder(order, limit);
        orderMap[orderId] = order;
        
        // Schedule matching after adding order
        OrderRequest matchRequest;
        matchRequest.operation = OrderOperation::Match;
        orderQueue.push(matchRequest);
    }
    
    bool processCancelOrder(int orderId) {
        std::lock_guard<std::mutex> lock(bookMutex);
        
        auto it = orderMap.find(orderId);
        if (it == orderMap.end()) {
            return false;
        }
        
        auto order = it->second;
        
        // Mark order as inactive atomically
        if (!order->active.exchange(false)) {
            // Order already inactive
            return false;
        }
        
        auto limitPtr = order->parentLimit.lock();
        if (!limitPtr) {
            orderMap.erase(orderId);
            return true;
        }
        
        {
            // Lock the specific limit level
            std::lock_guard<std::mutex> limitLock(limitPtr->limitMutex);
            
            // Update limit size and count atomically
            limitPtr->size.fetch_sub(order->shares);
            limitPtr->orderCount.fetch_sub(1);
            
            // Update linked list pointers
            auto prevOrder = order->prev.lock();
            if (prevOrder) {
                prevOrder->next = order->next;
            } else if (limitPtr->headOrder == order) {
                limitPtr->headOrder = order->next;
            }
            
            if (order->next) {
                if (auto nextOrder = order->next) {
                    nextOrder->prev = order->prev;
                }
            } else if (limitPtr->tailOrder == order) {
                limitPtr->tailOrder = prevOrder;
            }
        }
        
        // Remove order from map
        orderMap.erase(orderId);
        
        return true;
    }
    
    bool processModifyOrder(int orderId, int newShares) {
        std::lock_guard<std::mutex> lock(bookMutex);
        
        auto it = orderMap.find(orderId);
        if (it == orderMap.end()) {
            return false;
        }
        
        auto order = it->second;
        
        // Check if order is active
        if (!order->active.load()) {
            return false;
        }
        
        auto limitPtr = order->parentLimit.lock();
        if (!limitPtr) {
            return false;
        }
        
        {
            // Lock the specific limit level
            std::lock_guard<std::mutex> limitLock(limitPtr->limitMutex);
            
            // Update limit size atomically
            int oldShares = order->shares;
            limitPtr->size.fetch_add(newShares - oldShares);
            
            // Update order shares
            order->shares = newShares;
        }
        
        // Schedule matching after modifying order
        OrderRequest matchRequest;
        matchRequest.operation = OrderOperation::Match;
        orderQueue.push(matchRequest);
        
        return true;
    }
    
    void processMatching() {
        std::lock_guard<std::mutex> lock(bookMutex);
        
        if (!buyTree || !sellTree) {
            return;
        }
        
        // Find best bid and ask prices
        auto bestBid = buyTree;
        while (bestBid->rightChild) {
            bestBid = bestBid->rightChild;
        }
        
        auto bestAsk = sellTree;
        while (bestAsk->leftChild) {
            bestAsk = bestAsk->leftChild;
        }
        
        // Check if orders can match
        while (bestBid && bestAsk && bestBid->limitPrice >= bestAsk->limitPrice) {
            std::lock_guard<std::mutex> bidLock(bestBid->limitMutex);
            std::lock_guard<std::mutex> askLock(bestAsk->limitMutex);
            
            auto buyOrder = bestBid->headOrder;
            auto sellOrder = bestAsk->headOrder;
            
            // Both orders should be active
            if (!buyOrder || !sellOrder || !buyOrder->active.load() || !sellOrder->active.load()) {
                break;
            }
            
            // Calculate matched quantity
            int matchQty = std::min(buyOrder->shares, sellOrder->shares);
            int matchPrice = bestAsk->limitPrice; // Use ask price for matching
            
            // Create execution report
            ExecutionReport report;
            report.buyOrderId = buyOrder->idNumber;
            report.sellOrderId = sellOrder->idNumber;
            report.price = matchPrice;
            report.quantity = matchQty;
            report.timestamp = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            executionReports.push(report);
            
            // Update order quantities
            buyOrder->shares -= matchQty;
            sellOrder->shares -= matchQty;
            
            // Update limit sizes
            bestBid->size.fetch_sub(matchQty);
            bestAsk->size.fetch_sub(matchQty);
            
            // Remove or update orders
            bool removeBuyOrder = (buyOrder->shares == 0);
            bool removeSellOrder = (sellOrder->shares == 0);
            
            if (removeBuyOrder) {
                bestBid->headOrder = buyOrder->next;
                if (bestBid->headOrder) {
                    bestBid->headOrder->prev = std::weak_ptr<Order>();
                } else {
                    bestBid->tailOrder = nullptr;
                }
                bestBid->orderCount.fetch_sub(1);
                buyOrder->active.store(false);
                orderMap.erase(buyOrder->idNumber);
            }
            
            if (removeSellOrder) {
                bestAsk->headOrder = sellOrder->next;
                if (bestAsk->headOrder) {
                    bestAsk->headOrder->prev = std::weak_ptr<Order>();
                } else {
                    bestAsk->tailOrder = nullptr;
                }
                bestAsk->orderCount.fetch_sub(1);
                sellOrder->active.store(false);
                orderMap.erase(sellOrder->idNumber);
            }
            
            // Check if we need to move to next price levels
            if (bestBid->orderCount.load() == 0) {
                // Find next best bid
                if (bestBid->leftChild) {
                    bestBid = bestBid->leftChild;
                    while (bestBid->rightChild) {
                        bestBid = bestBid->rightChild;
                    }
                } else {
                    auto current = bestBid;
                    bestBid = current->parent.lock();
                    while (bestBid && bestBid->rightChild == current) {
                        current = bestBid;
                        bestBid = bestBid->parent.lock();
                    }
                }
            }
            
            if (bestAsk->orderCount.load() == 0) {
                // Find next best ask
                if (bestAsk->rightChild) {
                    bestAsk = bestAsk->rightChild;
                    while (bestAsk->leftChild) {
                        bestAsk = bestAsk->leftChild;
                    }
                } else {
                    auto current = bestAsk;
                    bestAsk = current->parent.lock();
                    while (bestAsk && bestAsk->leftChild == current) {
                        current = bestAsk;
                        bestAsk = bestAsk->parent.lock();
                    }
                }
            }
            
            // Break if no more orders to match
            if (!bestBid || !bestAsk || bestBid->orderCount.load() == 0 || bestAsk->orderCount.load() == 0 || 
                bestBid->limitPrice < bestAsk->limitPrice) {
                break;
            }
        }
    }
    
public:
    OrderBook() : buyTree(nullptr), sellTree(nullptr) {}
    
    ~OrderBook() {
        stop();
        // Smart pointers will handle memory cleanup automatically
    }
    
    // Start order processing
    void start() {
        bool expected = false;
        if (running.compare_exchange_strong(expected, true)) {
            matchingThread = std::thread(&OrderBook::processOrderQueue, this);
        }
    }
    
    // Stop order processing
    void stop() {
        running.store(false);
        orderQueue.shutdown();
        if (matchingThread.joinable()) {
            matchingThread.join();
        }
    }
    
    // API methods - now thread-safe
    int addOrder(bool isBuy, int shares, int price) {
        if (!running.load()) {
            return -1;
        }
        
        int orderId = nextOrderId.fetch_add(1);
        int entryTime = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        OrderRequest request;
        request.operation = OrderOperation::Add;
        request.orderId = orderId;
        request.isBuy = isBuy;
        request.shares = shares;
        request.price = price;
        request.entryTime = entryTime;
        
        orderQueue.push(request);
        return orderId;
    }
    
    bool cancelOrder(int orderId) {
        if (!running.load()) {
            return false;
        }
        
        OrderRequest request;
        request.operation = OrderOperation::Cancel;
        request.orderId = orderId;
        
        orderQueue.push(request);
        return true;
    }
    
    bool modifyOrder(int orderId, int newShares) {
        if (!running.load()) {
            return false;
        }
        
        OrderRequest request;
        request.operation = OrderOperation::Modify;
        request.orderId = orderId;
        request.shares = newShares;
        
        orderQueue.push(request);
        return true;
    }
    
    // Thread-safe query methods
    int getBestBid() const {
        std::lock_guard<std::mutex> lock(bookMutex);
        if (!buyTree) return 0;
        
        auto current = buyTree;
        while (current->rightChild) {
            current = current->rightChild;
        }
        return current->limitPrice;
    }
    
    int getBestAsk() const {
        std::lock_guard<std::mutex> lock(bookMutex);
        if (!sellTree) return 0;
        
        auto current = sellTree;
        while (current->leftChild) {
            current = current->leftChild;
        }
        return current->limitPrice;
    }
    
    int getVolumeAtPrice(int price) const {
        std::lock_guard<std::mutex> lock(bookMutex);
        auto it = limitMap.find(price);
        if (it == limitMap.end()) {
            return 0;
        }
        return it->second->size.load();
    }
    
    // Get next execution report
    bool getNextExecution(ExecutionReport& report) {
        return executionReports.try_pop(report);
    }
    
    // Register a callback function for execution reports
    void registerExecutionCallback(std::function<void(const ExecutionReport&)> callback) {
        // Start a thread to process execution reports
        std::thread([this, callback]() {
            while (running.load() || !executionReports.empty()) {
                ExecutionReport report;
                executionReports.wait_and_pop(report);
                if (callback && running.load()) {
                    callback(report);
                }
            }
        }).detach();
    }
    
    // Prevent copying
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
};

} // namespace OrderBook

// Non-member utility functions
void printOrderDetails(const std::shared_ptr<OrderBook::Order>& order);
void printLimitDetails(const std::shared_ptr<OrderBook::Limit>& limit);
void runThreadedTest();