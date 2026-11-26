#ifndef __HX2_INV_UI_H
#define __HX2_INV_UI_H

/* Inventory screen behind the m_inventory menu state (docs/items-inventory-spec.md).
 *
 * Phase 1 of the inventory slice: the read-only screen over the protocol-21
 * extended inventory.  The engine menu state (m_inventory) owns the state
 * machine and calls into here for drawing, hit-testing and navigation; this
 * file owns the layout and the reads that go with it.
 *
 * Reads go through the same extended-inventory walk the inv_* slot builtins
 * (#205-214) use, so the screen, the action queue and the storage cannot
 * disagree about what an item is. */

/* One extended-inventory page is 32 slots; the screen walks pages. */
#define	INV_SCREEN_PAGES		8
#define	INV_SCREEN_SLOTS		32

/* Equipment slots live on page 0; slots 10-12 are the equipped rune sockets. */
#define	INV_SCREEN_EQUIP_SLOTS	13

/* Screen state owned by the m_inventory menu state and passed in/out. */
typedef struct inv_screen_t
{
	client_t	*client;	/* whose inventory is on screen */
	int			page;		/* page being viewed / selection page */
	int			slot;		/* selection slot on that page */
} inv_screen_t;

void INV_ScreenInit (inv_screen_t *s, client_t *client);
void INV_ScreenShutdown (inv_screen_t *s);
void INV_ScreenDraw (const inv_screen_t *s);
int		INV_ScreenKey (const inv_screen_t *s, int key);
int		INV_ScreenClick (const inv_screen_t *s, int vx, int vy);
void	INV_ScreenHover (const inv_screen_t *s, int vx, int vy);

#endif	/* __HX2_INV_UI_H */
