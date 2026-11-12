/*
 * <linux/acpi.h> for ath10k's mac.c, under Linux's lower-case names (FreeBSD's ACPICA has its own): there is no
 * ACPI on the phone, and every call here says "not found", as Linux's do where a device has no ACPI handle.
 */
#ifndef _LINUXKPI_LINUX_ACPI_H_
#define _LINUXKPI_LINUX_ACPI_H_
typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;
typedef u32 acpi_object_type;
#define AE_OK                   0
#define AE_NOT_FOUND            5
#define ACPI_SUCCESS(s)         ((s) == AE_OK)
#define ACPI_FAILURE(s)         ((s) != AE_OK)
#define ACPI_ALLOCATE_BUFFER    ((size_t)-1)
#define ACPI_TYPE_INTEGER       0x01
#define ACPI_TYPE_STRING        0x02
#define ACPI_TYPE_BUFFER        0x03
#define ACPI_TYPE_PACKAGE       0x04
union acpi_object {
	acpi_object_type type;
	struct {
		acpi_object_type type;
		u64 value;
	} integer;
	struct {
		acpi_object_type type;
		u32 length;
		char *pointer;
	} string;
	struct {
		acpi_object_type type;
		u32 length;
		u8 *pointer;
	} buffer;
	struct {
		acpi_object_type type;
		u32 count;
		union acpi_object *elements;
	} package;
};
struct acpi_buffer {
	size_t length;
	void *pointer;
};
struct acpi_object_list;
#define ACPI_HANDLE(dev)        ((acpi_handle)0)
#define ACPI_COMPANION(dev)     ((void *)0)
#define has_acpi_companion(dev) (false)
static inline acpi_status
acpi_get_handle(acpi_handle parent, acpi_string pathname, acpi_handle *handle)
{
	(void)parent;
	(void)pathname;
	*handle = 0;
	return AE_NOT_FOUND;
}
static inline acpi_status
acpi_evaluate_object(acpi_handle handle, acpi_string pathname, struct acpi_object_list *args, struct acpi_buffer *ret)
{
	(void)handle;
	(void)pathname;
	(void)args;
	(void)ret;
	return AE_NOT_FOUND;
}
#endif
