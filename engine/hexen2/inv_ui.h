#ifndef __HX2_INV_UI_H
#define __HX2_INV_UI_H

/* Inventory screen overlay (docs/items-inventory-spec.md).
 *
 * The screen is a real-time HUD overlay, not a modal menu: the world keeps
 * ticking while it is open.  menu.c owns the key_dest switch and calls into
 * here for drawing, hit-testing and navigation; this file owns the layout and
 * the reads that go with it.
 *
 * Reads go through the same extended-inventory pages the inv_* slot builtins
 * (#205-214) use, so the screen, the action queue and the storage cannot
 * disagree about what an item is. */

/* One extended-inventory page is 32 slots; the screen walks pages. */
#define	INV_SCREEN_PAGES		8
#define	INV_SCREEN_SLOTS		32

/* Equipment slots live on page 0.  Slots 10-11 are the two guaranteed minor
 * rune cells; 12-13 and 14-15 are the two split-capable major rune columns. */
#define	INV_SCREEN_EQUIP_SLOTS	16

/* Screen state owned by the m_inventory overlay and passed in/out. */
typedef struct inv_screen_t
{
	client_state_t	*client;	/* whose inventory is on screen */
	int			page;		/* selected page */
	int			slot;		/* selected slot on that page */
	int			hover_page;	/* pointer hover, if any */
	int			hover_slot;
	int			hover_valid;

	int			drag_valid;	/* left-button drag in progress */
	int			drag_page;
	int			drag_slot;
	int			drop_valid;	/* resolved drop target cell */
	int			drop_page;
	int			drop_slot;
	int			pointer_x;
	int			pointer_y;
} inv_screen_t;

void INV_ScreenInit (inv_screen_t *s, client_state_t *client);
void INV_ScreenShutdown (inv_screen_t *s);
void INV_ScreenDraw (const inv_screen_t *s);
int		INV_ScreenKey (inv_screen_t *s, int key);
int		INV_ScreenClick (inv_screen_t *s, int vx, int vy);
void	INV_ScreenHover (inv_screen_t *s, int vx, int vy);
void	INV_ScreenDragStart (inv_screen_t *s, int vx, int vy);
void	INV_ScreenDragThink (inv_screen_t *s, int vx, int vy, int down);

#endif	/* __HX2_INV_UI_H */
