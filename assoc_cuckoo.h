/* associative array */
void assoc2_init(const int hashpower_init);
item *assoc2_find(const char *key, const size_t nkey, const uint32_t hv);
int assoc2_insert(item *item, const uint32_t hv);
void assoc2_delete(const char *key, const size_t nkey, const uint32_t hv);
/* void do_assoc_move_next_bucket(void); */
/* int start_assoc_maintenance_thread(void); */
/* void stop_assoc_maintenance_thread(void); */

void assoc2_destroy(void);
void assoc2_pre_bench(void);
void assoc2_post_bench(void);
