#include <iostream>
#include <unordered_map>
#include <list>
#include <utility>
#include <boost/optional.hpp>
#include <mutex>

using namespace std; 

template<class K, class T>
class LRUCache 
{ 
public: 

LRUCache(std::size_t n) 
{ 
    csize = n; 
}

// get the stored value by key
boost::optional<T> get(K const & key)
{
  std::lock_guard<std::mutex> lock(cache_mutex);
 
    if(is_in_cache(key))
    {
        move_element_to_front(key);
        return boost::optional<T>(std::get<1>(storage_list.front()));
    }
    
    return boost::optional<T>();
}

// store key value pair, return a pair <is_updated, key_erased>
// key_erased is the key of the item that has been erased due to LRU strategy
// if key_erased = "", no item has been removed, which indicates that the cache is not full before storing
std::pair<bool, K> store(K const & key, T const & value)
{   
    // if key is already in the cache, update the associate value. In the ranking list move the key to the front.
  std::lock_guard<std::mutex> lock(cache_mutex);
 
  if(is_in_cache(key))
    {
        auto const it =  lookup_map[key];
        storage_list.erase(it);
        lookup_map.erase(key);

        storage_list.push_front(std::pair<K, T>(key, value));
        lookup_map[key] = storage_list.begin();

        return std::make_pair<bool, K>(true, K());
    }
    else
    {
        // if key is not in the cache
        if(is_full())
        {
            // if the cache is full, remove the last key, value pair from storage list, remove the last key from lookup_map, add new key, value pair to the storage, add new key to the front of ranking list
            K last_element_key = std::get<0>(storage_list.back());
            storage_list.pop_back();
            lookup_map.erase(last_element_key);
            
            storage_list.push_front(std::pair<K, T>(key, value));
            lookup_map[key] = storage_list.begin();

            return std::make_pair<bool, K>(false, K(last_element_key));
        }
        else
        {
            // else, add new key value pair to storage, add new key to the front of ranking list.
            storage_list.push_front(std::pair<K, T>(key, value));
            lookup_map[key] = storage_list.begin();

            return std::make_pair<bool, K>(false, K());
        }
    }
}

// display the key of cache items in the order of storage_list 
void display() 
{ 
    auto it = storage_list.begin();
    int order = 0;
    for (; it != storage_list.end(); ++it, order++)
    {
        std::cout << "rank [" << order << "] : " << std::get<0>(*it) << "\n";
    }
}

private:
// if the key exist, move the value element associated with key to the front of storage list
void move_element_to_front(K const & key)
{
    if(is_in_cache(key))
    {
        auto const it = lookup_map[key];
        storage_list.splice(storage_list.begin(), storage_list, it);
    }
}

// check whether key, value pair exists in the system
bool is_in_cache(K const & key)
{
    return lookup_map.count(key) == 0 ? false : true;
}

//remove a key, value pair from the storage_list, remove the key in the lookup_map.
void remove(K const & key)
{   
    if(is_in_cache(key))
    {   
        auto const it = lookup_map[key];
        storage_list.erase(it);
        lookup_map.erase(key);
    }        
}

// return true if cache is full
bool is_full()
{
    return lookup_map.size() == csize;
}

private:
    // store key & value pair, maintain the order of visit, provide O(1) visit to the tail element
    list<std::pair<K, T> > storage_list;
     
    // provide O(1) lookup 
    unordered_map<K, typename list<std::pair<K, T> >::iterator > lookup_map;
    std::size_t csize; //maximum capacity of cache 
  std::mutex cache_mutex;
}; 

/*
//  unit test
int main() 
{ 
    LRUCache<std::string, int> lru_cache(4);

    lru_cache.display();


    std::string key1("1st key");
    std::string key2("2nd key");
    std::string key3("3rd key");
    std::string key4("4th key");
    std::string key5("5th key");
    int value1(1);
    int value2(2);
    int value3(3);
    int value4(4);
    int value5(5);

    lru_cache.store(key1, value1);
    auto no_removed_nor_updated = lru_cache.store(key2, value2);
    lru_cache.store(key3, value3);
    lru_cache.store(key4, value4);
    auto removed_key = lru_cache.store(key5, value5);
    auto updated_key = lru_cache.store(key5, value5);

    assert(std::get<0>(no_removed_nor_updated) == false && std::get<1>(no_removed_nor_updated) == "");
    assert(std::get<0>(removed_key) == false && std::get<1>(removed_key) != "");
    assert(std::get<0>(updated_key) == true && std::get<1>(updated_key) == "");

    lru_cache.display();

    auto get_from_cache = [&lru_cache](std::string key)
    {
        auto result = lru_cache.get(key);
        auto result_in_string = result == boost::none ? "None" : std::to_string(*result);
        std::cout << "get [" << key << "]: " << result_in_string << "\n";
    };

    get_from_cache(key1);
    get_from_cache(key2);
    get_from_cache(key4);

    lru_cache.display();
  
    return 0; 
}

*/
