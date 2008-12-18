#include "tpcctables.h"

#include <algorithm>
#include <vector>

#include "assert.h"
#include "stlutil.h"

using std::vector;

bool CustomerByNameOrdering::operator()(const Customer* a, const Customer* b) {
    if (a->c_w_id < b->c_w_id) return true;
    if (a->c_w_id > b->c_w_id) return false;
    assert(a->c_w_id == b->c_w_id);

    if (a->c_d_id < b->c_d_id) return true;
    if (a->c_d_id > b->c_d_id) return false;
    assert(a->c_d_id == b->c_d_id);

    int diff = strcmp(a->c_last, b->c_last);
    if (diff < 0) return true;
    if (diff > 0) return false;
    assert(diff == 0);

    // Finally delegate to c_first
    return strcmp(a->c_first, b->c_first) < 0;
}

TPCCTables::~TPCCTables() {
    // TODO: Clean up the b-trees.
    STLDeleteValues(&neworders_);
    STLDeleteElements(&customers_by_name_);
    STLDeleteElements(&history_);
}

int TPCCTables::stockLevel(int32_t warehouse_id, int32_t district_id, int32_t threshold) {
    /* EXEC SQL SELECT d_next_o_id INTO :o_id FROM district
        WHERE d_w_id=:w_id AND d_id=:d_id; */
    //~ printf("stock level %d %d %d\n", warehouse_id, district_id, threshold);
    District* d = findDistrict(warehouse_id, district_id);
    int32_t o_id = d->d_next_o_id;

    /* EXEC SQL SELECT COUNT(DISTINCT (s_i_id)) INTO :stock_count FROM order_line, stock
        WHERE ol_w_id=:w_id AND ol_d_id=:d_id AND ol_o_id<:o_id AND ol_o_id>=:o_id-20
            AND s_w_id=:w_id AND s_i_id=ol_i_id AND s_quantity < :threshold;*/

    
    // retrieve up to 300 tuples from order line, using ( [o_id-20, o_id), d_id, w_id, [1, 15])
    //   and for each retrieved tuple, read the corresponding stock tuple using (ol_i_id, w_id)
    // NOTE: This is a cheat because it hard codes the maximum number of orders.
    // We really should use the ordered b-tree index to find (0, o_id-20, d_id, w_id) then iterate
    // until the end. This will also do less work (wasted finds). Since this is only 4%, it probably
    // doesn't matter much

    // TODO: Test the performance more carefully. I tried: std::set, std::hash_set, std::vector
    // with linear search, and std::vector with binary search using std::lower_bound. The best
    // seemed to be to simply save all the s_i_ids, then sort and eliminate duplicates at the end.
    std::vector<int32_t> s_i_ids;
    // Average size is more like ~30.
    s_i_ids.reserve(300);

    // Iterate over [o_id-20, o_id)
    for (int order_id = o_id - STOCK_LEVEL_ORDERS; order_id < o_id; ++order_id) {
        // HACK: We shouldn't rely on MAX_OL_CNT. See comment above.
        for (int line_number = 1; line_number <= Order::MAX_OL_CNT; ++line_number) {
            OrderLine* line = findOrderLine(warehouse_id, district_id, order_id, line_number);
            if (line == NULL) {
                // We can break since we have reached the end of the lines for this order.
                // TODO: A btree iterate in (w_id, d_id, o_id) order would be a clean way to do this
#ifndef NDEBUG
                for (int test_line_number = line_number + 1; line_number < Order::MAX_OL_CNT; ++line_number) {
                    assert(findOrderLine(warehouse_id, district_id, order_id, test_line_number) == NULL);
                }
#endif
                break;
            }

            // Check if s_quantity < threshold
            Stock* stock = findStock(warehouse_id, line->ol_i_id);
            if (stock->s_quantity < threshold) {
                s_i_ids.push_back(line->ol_i_id);
            }
        }
    }

    // Filter out duplicate s_i_id: multiple order lines can have the same item
    std::sort(s_i_ids.begin(), s_i_ids.end());
    int num_distinct = 0;
    int32_t last = -1;  // NOTE: This relies on -1 being an invalid s_i_id
    for (size_t i = 0; i < s_i_ids.size(); ++i) {
        if (s_i_ids[i] != last) {
            last = s_i_ids[i];
            num_distinct += 1;
        }
    }

    return num_distinct;
}

void TPCCTables::orderStatus(int32_t warehouse_id, int32_t district_id, int32_t customer_id, OrderStatusOutput* output) {
    //~ printf("order status %d %d %d\n", warehouse_id, district_id, customer_id);
    internalOrderStatus(findCustomer(warehouse_id, district_id, customer_id), output);
}

void TPCCTables::orderStatus(int32_t warehouse_id, int32_t district_id, const char* c_last, OrderStatusOutput* output) {
    //~ printf("order status %d %d %s\n", warehouse_id, district_id, c_last);
    Customer* customer = findCustomerByName(warehouse_id, district_id, c_last);
    internalOrderStatus(customer, output);
}

void TPCCTables::internalOrderStatus(Customer* customer, OrderStatusOutput* output) {
    output->c_id = customer->c_id;
    // retrieve from customer: balance, first, middle, last
    output->c_balance = customer->c_balance;
    strcpy(output->c_first, customer->c_first);
    strcpy(output->c_middle, customer->c_middle);
    strcpy(output->c_last, customer->c_last);

    // Find the row in the order table with largest o_id
    Order* order = findLastOrderByCustomer(customer->c_w_id, customer->c_d_id, customer->c_id);
    output->o_id = order->o_id;
    output->o_carrier_id = order->o_carrier_id;
    strcpy(output->o_entry_d, order->o_entry_d);

    output->lines.resize(order->o_ol_cnt);
    for (int32_t line_number = 1; line_number <= order->o_ol_cnt; ++line_number) {
        OrderLine* line = findOrderLine(customer->c_w_id, customer->c_d_id, order->o_id, line_number);
        output->lines[line_number-1].ol_i_id = line->ol_i_id;
        output->lines[line_number-1].ol_supply_w_id = line->ol_supply_w_id;
        output->lines[line_number-1].ol_quantity = line->ol_quantity;
        output->lines[line_number-1].ol_amount = line->ol_amount;
        strcpy(output->lines[line_number-1].ol_delivery_d, line->ol_delivery_d);
    }
#ifndef NDEBUG
    // Verify that none of the other OrderLines exist.
    for (int32_t line_number = order->o_ol_cnt+1; line_number <= Order::MAX_OL_CNT; ++line_number) {
        assert(findOrderLine(customer->c_w_id, customer->c_d_id, order->o_id, line_number) == NULL);
    }
#endif
}

bool TPCCTables::newOrder(int32_t warehouse_id, int32_t district_id, int32_t customer_id,
        const std::vector<NewOrderItem>& items, const char* now, NewOrderOutput* output) {
    //~ printf("new order %d %d %d %d %s\n", warehouse_id, district_id, customer_id, items.size(), now);
    // 2.4.3.4. requires that we display c_last, c_credit, and o_id for rolled back transactions:
    // read those values first
    District* d = findDistrict(warehouse_id, district_id);
    output->d_tax = d->d_tax;
    output->o_id = d->d_next_o_id;
    assert(findOrder(warehouse_id, district_id, output->o_id) == NULL);

    Customer* c = findCustomer(warehouse_id, district_id, customer_id);
    assert(sizeof(output->c_last) == sizeof(c->c_last));
    memcpy(output->c_last, c->c_last, sizeof(output->c_last));
    memcpy(output->c_credit, c->c_credit, sizeof(output->c_credit));
    output->c_discount = c->c_discount;

    // CHEAT: Validate all items to see if we will need to abort
    vector<Item*> item_tuples(items.size());
    bool all_local = true;
    for (int i = 0; i < items.size(); ++i) {
        item_tuples[i] = findItem(items[i].i_id);
        if (item_tuples[i] == NULL) {
            strcpy(output->status, NewOrderOutput::INVALID_ITEM_STATUS);
            return false;
        }
        all_local = all_local && items[i].ol_supply_w_id == warehouse_id;
    }

    // We will not abort: update the status and the database state
    output->status[0] = '\0';

    // Modify the order id to assign it
    d->d_next_o_id += 1;

    Warehouse* w = findWarehouse(warehouse_id);
    output->w_tax = w->w_tax;

    Order order;
    order.o_w_id = warehouse_id;
    order.o_d_id = district_id;
    order.o_id = output->o_id;
    order.o_c_id = customer_id;
    order.o_carrier_id = Order::NULL_CARRIER_ID;
    order.o_ol_cnt = static_cast<int32_t>(items.size());
    order.o_all_local = all_local ? 1 : 0;
    strcpy(order.o_entry_d, now);
    assert(strlen(order.o_entry_d) == DATETIME_SIZE);
    insertOrder(order);
    insertNewOrder(warehouse_id, district_id, output->o_id);

    OrderLine line;
    line.ol_o_id = output->o_id;
    line.ol_d_id = district_id;
    line.ol_w_id = warehouse_id;
    memset(line.ol_delivery_d, 0, DATETIME_SIZE+1);

    output->items.resize(items.size());
    output->total = 0;
    for (int i = 0; i < items.size(); ++i) {
        line.ol_number = i+1;
        line.ol_i_id = items[i].i_id;
        line.ol_supply_w_id = items[i].ol_supply_w_id;
        line.ol_quantity = items[i].ol_quantity;

        // Read and update stock
        Stock* stock = findStock(items[i].ol_supply_w_id, items[i].i_id);
        if (stock->s_quantity >= items[i].ol_quantity + 10) {
            stock->s_quantity -= items[i].ol_quantity;
        } else {
            stock->s_quantity = stock->s_quantity - items[i].ol_quantity + 91;
        }
        output->items[i].s_quantity = stock->s_quantity;
        assert(sizeof(line.ol_dist_info) == sizeof(stock->s_dist[district_id]));
        memcpy(line.ol_dist_info, stock->s_dist[district_id], sizeof(line.ol_dist_info));
        stock->s_ytd += items[i].ol_quantity;
        stock->s_order_cnt += 1;
        if (items[i].ol_supply_w_id != warehouse_id) {
            // remote order
            stock->s_remote_cnt += 1;
        }
        bool stock_is_original = (strstr(stock->s_data, "ORIGINAL") != NULL);

        assert(sizeof(output->items[i].i_name) == sizeof(item_tuples[i]->i_name));
        memcpy(output->items[i].i_name, item_tuples[i]->i_name, sizeof(output->items[i].i_name));
        output->items[i].i_price = item_tuples[i]->i_price;
        output->items[i].ol_amount =
                static_cast<float>(items[i].ol_quantity) * item_tuples[i]->i_price;
        line.ol_amount = output->items[i].ol_amount;
        output->total += output->items[i].ol_amount;
        if (stock_is_original && strstr(item_tuples[i]->i_data, "ORIGINAL") != NULL) {
            output->items[i].brand_generic = NewOrderOutput::ItemInfo::BRAND;
        } else {
            output->items[i].brand_generic = NewOrderOutput::ItemInfo::GENERIC;
        }
        insertOrderLine(line);
    }

    return true;
}

void TPCCTables::payment(int32_t warehouse_id, int32_t district_id, int32_t c_warehouse_id,
        int32_t c_district_id, int32_t customer_id, float h_amount, const char* now,
        PaymentOutput* output) {
    //~ printf("payment %d %d %d %d %d %f %s\n", warehouse_id, district_id, c_warehouse_id, c_district_id, customer_id, h_amount, now);
    Customer* customer = findCustomer(c_warehouse_id, c_district_id, customer_id);
    internalPayment(warehouse_id, district_id, customer, h_amount, now, output);
}


void TPCCTables::payment(int32_t warehouse_id, int32_t district_id, int32_t c_warehouse_id,
        int32_t c_district_id, const char* c_last, float h_amount, const char* now,
        PaymentOutput* output) {
    //~ printf("payment %d %d %d %d %s %f %s\n", warehouse_id, district_id, c_warehouse_id, c_district_id, c_last, h_amount, now);
    Customer* customer = findCustomerByName(c_warehouse_id, c_district_id, c_last);
    internalPayment(warehouse_id, district_id, customer, h_amount, now, output);
}

void TPCCTables::internalPayment(int32_t warehouse_id, int32_t district_id, Customer* c,
        float h_amount, const char* now, PaymentOutput* output) {
    Warehouse* w = findWarehouse(warehouse_id);
    w->w_ytd += h_amount;
    output->warehouse = *w;

    District* d = findDistrict(warehouse_id, district_id);
    d->d_ytd += h_amount;
    output->district = *d;

    c->c_balance -= h_amount;
    c->c_ytd_payment += h_amount;
    c->c_payment_cnt += 1;
    if (strcmp(c->c_credit, Customer::BAD_CREDIT) == 0) {
        // Bad credit: insert history into c_data
        static const int HISTORY_SIZE = Customer::MAX_DATA+1;
        char history[HISTORY_SIZE];
        int characters = snprintf(history, HISTORY_SIZE, "(%d, %d, %d, %d, %d, %.2f)\n",
                c->c_id, c->c_d_id, c->c_w_id, district_id, warehouse_id, h_amount);
        assert(characters < HISTORY_SIZE);

        // Perform the insert with a move and copy
        int current_keep = static_cast<int>(strlen(c->c_data));
        if (current_keep + characters > Customer::MAX_DATA) {
            current_keep = Customer::MAX_DATA - characters;
        }
        assert(current_keep + characters <= Customer::MAX_DATA);
        memmove(c->c_data+characters, c->c_data, current_keep);
        memcpy(c->c_data, history, characters);
        c->c_data[characters + current_keep] = '\0';
        assert(strlen(c->c_data) == characters + current_keep);
    }
    output->customer = *c;

    // Insert the line into the history table
    History h;
    h.h_w_id = warehouse_id;
    h.h_d_id = district_id;
    h.h_c_w_id = c->c_w_id;
    h.h_c_d_id = c->c_d_id;
    h.h_c_id = c->c_id;
    h.h_amount = h_amount;
    strcpy(h.h_date, now);
    strcpy(h.h_data, w->w_name);
    strcat(h.h_data, "    ");
    strcat(h.h_data, d->d_name);
    insertHistory(h);
}

// forward declaration for delivery
static int64_t makeNewOrderKey(int32_t w_id, int32_t d_id, int32_t o_id);

void TPCCTables::delivery(int32_t warehouse_id, int32_t carrier_id, const char* now,
        std::vector<DeliveryOrderInfo>* orders) {
    //~ printf("delivery %d %d %s\n", warehouse_id, carrier_id, now);
    orders->clear();
    for (int32_t d_id = 1; d_id <= District::NUM_PER_WAREHOUSE; ++d_id) {
        // Find and remove the lowest numbered order for the district
        int64_t key = makeNewOrderKey(warehouse_id, d_id, 1);
        NewOrderMap::iterator iterator = neworders_.lower_bound(key);
        NewOrder* neworder = NULL;
        if (iterator != neworders_.end()) {
            neworder = iterator->second;
            assert(neworder != NULL);
        }
        if (neworder == NULL || neworder->no_d_id != d_id || neworder->no_w_id != warehouse_id) {
            // No orders for this district
            // TODO: 2.7.4.2: If this occurs in max(1%, 1) of transactions, report it (???)
            continue;
        }
        assert(neworder->no_d_id == d_id && neworder->no_w_id == warehouse_id);
        int32_t o_id = neworder->no_o_id;
        neworders_.erase(iterator);
        delete neworder;

        DeliveryOrderInfo order;
        order.d_id = d_id;
        order.o_id = o_id;
        orders->push_back(order);

        Order* o = findOrder(warehouse_id, d_id, o_id);
        assert(o->o_carrier_id == Order::NULL_CARRIER_ID);
        o->o_carrier_id = carrier_id;

        float total = 0;
        // TODO: Select based on (w_id, d_id, o_id) rather than using ol_number?
        for (int32_t i = 1; i <= o->o_ol_cnt; ++i) {
            OrderLine* line = findOrderLine(warehouse_id, d_id, o_id, i);
            assert(0 == strlen(line->ol_delivery_d));
            strcpy(line->ol_delivery_d, now);
            assert(strlen(line->ol_delivery_d) == DATETIME_SIZE);
            total += line->ol_amount;
        }

        Customer* c = findCustomer(warehouse_id, d_id, o->o_c_id);
        c->c_balance += total;
        c->c_delivery_cnt += 1;
    }
}

template <typename T>
static T* insert(BPlusTree<int32_t, T*, TPCCTables::KEYS_PER_INTERNAL, TPCCTables::KEYS_PER_LEAF>* tree, int32_t key, const T& item) {
    assert(!tree->find(key));
    T* copy = new T(item);
    tree->insert(key, copy);
    return copy;
}

template <typename T>
static T* find(const BPlusTree<int32_t, T*, TPCCTables::KEYS_PER_INTERNAL, TPCCTables::KEYS_PER_LEAF>& tree, int32_t key) {
    T* output = NULL;
    if (tree.find(key, &output)) {
        return output;
    }
    return NULL;
}

void TPCCTables::insertItem(const Item& item) {
    assert(item.i_id == items_.size() + 1);
    items_.push_back(item);
}
Item* TPCCTables::findItem(int32_t id) {
    assert(1 <= id);
    id -= 1;
    if (id >= items_.size()) return NULL;
    return &items_[id];
}

void TPCCTables::insertWarehouse(const Warehouse& w) {
    insert(&warehouses_, w.w_id, w);
}
Warehouse* TPCCTables::findWarehouse(int32_t id) {
    return find(warehouses_, id);
}

static int32_t makeStockKey(int32_t w_id, int32_t s_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= s_id && s_id <= Stock::NUM_STOCK_PER_WAREHOUSE);
    int32_t id = s_id + (w_id * Stock::NUM_STOCK_PER_WAREHOUSE);
    assert(id >= 0);
    return id;
}

void TPCCTables::insertStock(const Stock& stock) {
    insert(&stock_, makeStockKey(stock.s_w_id, stock.s_i_id), stock);
}
Stock* TPCCTables::findStock(int32_t w_id, int32_t s_id) {
    return find(stock_, makeStockKey(w_id, s_id));
}

static int32_t makeDistrictKey(int32_t w_id, int32_t d_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    int32_t id = d_id + (w_id * District::NUM_PER_WAREHOUSE);
    assert(id >= 0);
    return id;
}

void TPCCTables::insertDistrict(const District& district) {
    insert(&districts_, makeDistrictKey(district.d_w_id, district.d_id), district);
}
District* TPCCTables::findDistrict(int32_t w_id, int32_t d_id) {
    return find(districts_, makeDistrictKey(w_id, d_id));
}

static int32_t makeCustomerKey(int32_t w_id, int32_t d_id, int32_t c_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    assert(1 <= c_id && c_id <= Customer::NUM_PER_DISTRICT);
    int32_t id = (w_id * District::NUM_PER_WAREHOUSE + d_id)
            * Customer::NUM_PER_DISTRICT + c_id;
    assert(id >= 0);
    return id;
}

void TPCCTables::insertCustomer(const Customer& customer) {
    Customer* c = insert(&customers_, makeCustomerKey(customer.c_w_id, customer.c_d_id, customer.c_id), customer);
    assert(customers_by_name_.find(c) == customers_by_name_.end());
    customers_by_name_.insert(c);
}
Customer* TPCCTables::findCustomer(int32_t w_id, int32_t d_id, int32_t c_id) {
    return find(customers_, makeCustomerKey(w_id, d_id, c_id));
}

Customer* TPCCTables::findCustomerByName(int32_t w_id, int32_t d_id, const char* c_last) {
    // select (w_id, d_id, *, c_last) order by c_first
    Customer c;
    c.c_w_id = w_id;
    c.c_d_id = d_id;
    strcpy(c.c_last, c_last);
    c.c_first[0] = '\0';
    CustomerByNameSet::const_iterator it = customers_by_name_.lower_bound(&c);
    assert(it != customers_by_name_.end());
    assert((*it)->c_w_id == w_id && (*it)->c_d_id == d_id && strcmp((*it)->c_last, c_last) == 0);

    // go to the "next" c_last
    // TODO: This is a GROSS hack. Can we do better?
    int length = static_cast<int>(strlen(c_last));
    if (length == Customer::MAX_LAST) {
        c.c_last[length-1] = static_cast<char>(c.c_last[length-1] + 1);
    } else {
        c.c_last[length] = 'A';
        c.c_last[length+1] = '\0';
    }
    CustomerByNameSet::const_iterator stop = customers_by_name_.lower_bound(&c);

    Customer* customer = NULL;
    // Choose position n/2 rounded up (1 based addressing) = floor((n-1)/2)
    if (it != stop) {
        CustomerByNameSet::const_iterator middle = it;
        ++it;
        int i = 0;
        while (it != stop) {
            // Increment the middle iterator on every second iteration
            if (i % 2 == 1) {
                ++middle;
            }
            assert(strcmp((*it)->c_last, c_last) == 0);
            ++it;
            ++i;
        }
        // There were i+1 matching last names
        customer = *middle;
    }
    
    assert(customer->c_w_id == w_id && customer->c_d_id == d_id &&
            strcmp(customer->c_last, c_last) == 0);
    return customer;
}

static int32_t makeOrderKey(int32_t w_id, int32_t d_id, int32_t o_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    assert(1 <= o_id && o_id <= Order::MAX_ORDER_ID);
    // TODO: This is bad for locality since o_id is in the most significant position. Larger keys?
    int32_t id = (o_id * District::NUM_PER_WAREHOUSE + d_id)
            * Warehouse::MAX_WAREHOUSE_ID + w_id;
    assert(id >= 0);
    return id;
}

static int64_t makeOrderByCustomerKey(int32_t w_id, int32_t d_id, int32_t c_id, int32_t o_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    assert(1 <= c_id && c_id <= Customer::NUM_PER_DISTRICT);
    assert(1 <= o_id && o_id <= Order::MAX_ORDER_ID);
    int32_t top_id = (w_id * District::NUM_PER_WAREHOUSE + d_id) * Customer::NUM_PER_DISTRICT
            + c_id;
    assert(top_id >= 0);
    int64_t id = (((int64_t) top_id) << 32) | o_id;
    assert(id > 0);
    return id;
}

void TPCCTables::insertOrder(const Order& order) {
    Order* tuple = insert(&orders_, makeOrderKey(order.o_w_id, order.o_d_id, order.o_id), order);
    // Secondary index based on customer id
    int64_t key = makeOrderByCustomerKey(order.o_w_id, order.o_d_id, order.o_c_id, order.o_id);
    assert(!orders_by_customer_.find(key));
    orders_by_customer_.insert(key, tuple);
}
Order* TPCCTables::findOrder(int32_t w_id, int32_t d_id, int32_t o_id) {
    return find(orders_, makeOrderKey(w_id, d_id, o_id));
}
Order* TPCCTables::findLastOrderByCustomer(const int32_t w_id, const int32_t d_id, const int32_t c_id) {
    Order* order = NULL;

    // Increment the (w_id, d_id, c_id) tuple
    int64_t key = makeOrderByCustomerKey(w_id, d_id, c_id, 1);
    key += ((int64_t)1) << 32;
    ASSERT(key > 0);

    bool found = orders_by_customer_.findLastLessThan(key, &order);
    ASSERT(!found || (order->o_w_id == w_id && order->o_d_id == d_id && order->o_c_id == c_id));
    return order;
}

static int32_t makeOrderLineKey(int32_t w_id, int32_t d_id, int32_t o_id, int32_t number) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    assert(1 <= o_id && o_id <= Order::MAX_ORDER_ID);
    assert(1 <= number && number <= Order::MAX_OL_CNT);
    // TODO: This may be bad for locality since o_id is in the most significant position. However,
    // Order status fetches all rows for one (w_id, d_id, o_id) tuple, so it may be fine,
    // but stock level fetches order lines for a range of (w_id, d_id, o_id) values
    int32_t id = ((o_id * District::NUM_PER_WAREHOUSE + d_id)
            * Warehouse::MAX_WAREHOUSE_ID + w_id) * Order::MAX_OL_CNT + number;
    assert(id >= 0);
    return id;
}

void TPCCTables::insertOrderLine(const OrderLine& orderline) {
    int32_t key = makeOrderLineKey(
            orderline.ol_w_id, orderline.ol_d_id, orderline.ol_o_id, orderline.ol_number);
    insert(&orderlines_, key, orderline);
}
OrderLine* TPCCTables::findOrderLine(int32_t w_id, int32_t d_id, int32_t o_id, int32_t number) {
    return find(orderlines_, makeOrderLineKey(w_id, d_id, o_id, number));
}

static int64_t makeNewOrderKey(int32_t w_id, int32_t d_id, int32_t o_id) {
    assert(1 <= w_id && w_id <= Warehouse::MAX_WAREHOUSE_ID);
    assert(1 <= d_id && d_id <= District::NUM_PER_WAREHOUSE);
    assert(1 <= o_id && o_id <= Order::MAX_ORDER_ID);
    int32_t upper_id = w_id * Warehouse::MAX_WAREHOUSE_ID + d_id;
    assert(upper_id > 0);
    int64_t id = static_cast<int64_t>(upper_id) << 32 | o_id;
    assert(id > 0);
    return id;
}

void TPCCTables::insertNewOrder(int32_t w_id, int32_t d_id, int32_t o_id) {
    NewOrder* neworder = new NewOrder();
    neworder->no_w_id = w_id;
    neworder->no_d_id = d_id;
    neworder->no_o_id = o_id;

    int64_t key = makeNewOrderKey(neworder->no_w_id, neworder->no_d_id, neworder->no_o_id);
    assert(neworders_.find(key) == neworders_.end());
    neworders_.insert(std::make_pair(key, neworder));
}
NewOrder* TPCCTables::findNewOrder(int32_t w_id, int32_t d_id, int32_t o_id) {
    NewOrderMap::const_iterator it = neworders_.find(makeNewOrderKey(w_id, d_id, o_id));
    if (it == neworders_.end()) return NULL;
    assert(it->second != NULL);
    return it->second;
}

void TPCCTables::insertHistory(const History& history) {
    History* h = new History(history);
    history_.push_back(h);
}