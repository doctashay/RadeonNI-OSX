#ifndef RDN_HOST_H
#define RDN_HOST_H
/* The imported BO owns the lease once accepted becomes nonzero. */
struct rdn_host_import {
 void *pages;
 void *lease;
 void (*release)(void *);
 int accepted;
};
#endif
