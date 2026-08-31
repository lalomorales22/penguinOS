// Writes the shipped buddy onto /int the first time the filesystem is empty.
// Call AFTER storage mounts and BEFORE the buddy is adopted. Never overwrites.
#ifndef EOS_SEED_BUDDY_H
#define EOS_SEED_BUDDY_H
void eos_seed_buddy(void);
#endif
