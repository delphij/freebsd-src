/* ThinLTO fixture, with l2.c: scale() is imported into l2.c's unit and inlined there, so the object carries l2.c's unit, then units for what it imported, referring to each other with DW_FORM_ref_addr */
struct pair { int lo; int hi; };
typedef struct pair pair_t;
int sum_pair(pair_t *p) { return p->lo + p->hi; }
int scale(int x) { pair_t q = { x, x * 2 }; return sum_pair(&q) * 3; }
