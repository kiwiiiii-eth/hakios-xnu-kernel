/* A stand-in for XNU's <pexpert/pexpert.h>, for compiling pexpert/gen/bootargs.c on the host (tools/args-builtin-test.py):
 * the types and the few functions that file uses. Nothing of the kernel's behaviour is here. */
#ifndef HAKIOS_HOST_PEXPERT_H
#define HAKIOS_HOST_PEXPERT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int boolean_t;
typedef int kern_return_t;
#define TRUE 1
#define FALSE 0
#define KERN_SUCCESS 0
#define kSuccess 0
typedef void *DTEntry;
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_len);
boolean_t PE_parse_boot_arg_str(const char *arg_string, char *arg_ptr, int strlen);
char *PE_boot_args(void);
int SecureDTLookupEntry(void *a, const char *b, DTEntry *c);
int SecureDTGetProperty(DTEntry a, const char *b, void const **c, unsigned int *d);
#endif
