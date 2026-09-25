#ifndef __CONTACTS_H__
#define __CONTACTS_H__

#include <stdbool.h>
#include <stddef.h>

/* Number -> name table built from the phonebook the phone sends over PBAP,
 * so an incoming call can show who is calling. Kept in RAM only. */

/* Empties the table before a fresh phonebook download. */
void contacts_reset(void);

/* Feeds one chunk of vCard text; chunks may split lines anywhere. */
void contacts_feed(const char *data, size_t len);

/* Finds the contact whose number matches (on the last up-to-10 digits,
 * so +91 98xxx and 098xxx both match). Returns false if none. */
bool contacts_lookup(const char *number, char *name, size_t name_size);

int contacts_count(void);

/* True if the last download had more numbers than memory allowed. */
bool contacts_full(void);

#endif /* __CONTACTS_H__ */
