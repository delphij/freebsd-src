/* see l1.c */
struct pair;
int scale(int);
int sum_pair(struct pair *);
struct pair *gp;
int use_scale(int n) { return scale(n) + scale(n + 1); }
int use_sum(struct pair *p) { return sum_pair(p); }
