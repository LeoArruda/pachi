#define DEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "debug.h"
#include "tactics.h"
#include "uct/dynkomi.h"
#include "uct/internal.h"
#include "uct/tree.h"


static void
generic_done(struct uct_dynkomi *d)
{
	if (d->data) free(d->data);
	free(d);
}


/* NONE dynkomi strategy - never fiddle with komi values. */

struct uct_dynkomi *
uct_dynkomi_init_none(struct uct *u, char *arg, struct board *b)
{
	struct uct_dynkomi *d = calloc2(1, sizeof(*d));
	d->uct = u;
	d->permove = NULL;
	d->persim = NULL;
	d->done = generic_done;
	d->data = NULL;

	if (arg) {
		fprintf(stderr, "uct: Dynkomi method none accepts no arguments\n");
		exit(1);
	}

	return d;
}


/* LINEAR dynkomi strategy - Linearly Decreasing Handicap Compensation. */
/* At move 0, we impose extra komi of handicap_count*handicap_value, then
 * we linearly decrease this extra komi throughout the game down to 0
 * at @moves moves. */

struct dynkomi_linear {
	int handicap_value;
	int moves;
	bool rootbased;
};

static float
linear_permove(struct uct_dynkomi *d, struct board *b, struct tree *tree)
{
	struct dynkomi_linear *l = d->data;
	if (b->moves >= l->moves)
		return 0;

	float base_komi = board_effective_handicap(b, l->handicap_value);
	float extra_komi = base_komi * (l->moves - b->moves) / l->moves;
	return extra_komi;
}

static float
linear_persim(struct uct_dynkomi *d, struct board *b, struct tree *tree, struct tree_node *node)
{
	struct dynkomi_linear *l = d->data;
	if (l->rootbased)
		return tree->extra_komi;
	/* We don't reuse computed value from tree->extra_komi,
	 * since we want to use value correct for this node depth.
	 * This also means the values will stay correct after
	 * node promotion. */
	return linear_permove(d, b, tree);
}

struct uct_dynkomi *
uct_dynkomi_init_linear(struct uct *u, char *arg, struct board *b)
{
	struct uct_dynkomi *d = calloc2(1, sizeof(*d));
	d->uct = u;
	d->permove = linear_permove;
	d->persim = linear_persim;
	d->done = generic_done;

	struct dynkomi_linear *l = calloc2(1, sizeof(*l));
	d->data = l;

	if (board_size(b) - 2 >= 19)
		l->moves = 200;
	l->handicap_value = 7;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "moves") && optval) {
				/* Dynamic komi in handicap game; linearly
				 * decreases to basic settings until move
				 * #optval. */
				l->moves = atoi(optval);
			} else if (!strcasecmp(optname, "handicap_value") && optval) {
				/* Point value of single handicap stone,
				 * for dynkomi computation. */
				l->handicap_value = atoi(optval);
			} else if (!strcasecmp(optname, "rootbased")) {
				/* If set, the extra komi applied will be
				 * the same for all simulations within a move,
				 * instead of being same for all simulations
				 * within the tree node. */
				l->rootbased = !optval || atoi(optval);
			} else {
				fprintf(stderr, "uct: Invalid dynkomi argument %s or missing value\n", optname);
				exit(1);
			}
		}
	}

	return d;
}


/* ADAPTIVE dynkomi strategy - Adaptive Situational Compensation */
/* We adapt the komi based on current situation:
 * (i) score-based: We maintain the average score outcome of our
 * games and adjust the komi by a fractional step towards the expected
 * score;
 * (ii) value-based: While winrate is above given threshold, adjust
 * the komi by a fixed step in the appropriate direction.
 * These adjustments can be
 * (a) Move-stepped, new extra komi value is always set only at the
 * beginning of the tree search for next move;
 * (b) Continuous, new extra komi value is periodically re-determined
 * and adjusted throughout a single tree search. */

struct dynkomi_adaptive {
	/* Do not take measured average score into regard for
	 * first @lead_moves - the variance is just too much.
	 * (Instead, we consider the handicap-based komi provided
	 * by linear dynkomi.) */
	int lead_moves;
	/* Maximum komi to pretend the opponent to give. */
	float max_losing_komi;
	float (*indicator)(struct uct_dynkomi *d, struct board *b, struct tree *tree, enum stone color);

	/* Value-based adaptation. */
	float zone_red, zone_green;
	int score_step;
	float score_step_byavg; // use portion of average score as increment
	bool use_komi_ratchet;
	int komi_ratchet_maxage;
	// runtime, not configuration:
	int komi_ratchet_age;
	float komi_ratchet;

	/* Score-based adaptation. */
	float (*adapter)(struct uct_dynkomi *d, struct board *b);
	float adapt_base; // [0,1)
	/* Sigmoid adaptation rate parameter; see below for details. */
	float adapt_phase; // [0,1]
	float adapt_rate; // [1,infty)
	bool adapt_aport; // alternative game portion determination
	/* Linear adaptation rate parameter. */
	int adapt_moves;
	float adapt_dir; // [-1,1]
};
#define TRUSTWORTHY_KOMI_PLAYOUTS 200

static float
adapter_sigmoid(struct uct_dynkomi *d, struct board *b)
{
	struct dynkomi_adaptive *a = d->data;
	/* Figure out how much to adjust the komi based on the game
	 * stage. The adaptation rate is 0 at the beginning,
	 * at game stage a->adapt_phase crosses though 0.5 and
	 * approaches 1 at the game end; the slope is controlled
	 * by a->adapt_rate. */
	float game_portion;
	if (!a->adapt_aport) {
		int total_moves = b->moves + 2 * board_estimated_moves_left(b);
		game_portion = (float) b->moves / total_moves;
	} else {
		int brsize = board_size(b) - 2;
		game_portion = 1.0 - (float) b->flen / (brsize * brsize);
	}
	float l = game_portion - a->adapt_phase;
	return 1.0 / (1.0 + exp(-a->adapt_rate * l));
}

static float
adapter_linear(struct uct_dynkomi *d, struct board *b)
{
	struct dynkomi_adaptive *a = d->data;
	/* Figure out how much to adjust the komi based on the game
	 * stage. We just linearly increase/decrease the adaptation
	 * rate for first N moves. */
	if (b->moves > a->adapt_moves)
		return 0;
	if (a->adapt_dir < 0)
		return 1 - (- a->adapt_dir) * b->moves / a->adapt_moves;
	else
		return a->adapt_dir * b->moves / a->adapt_moves;
}

static float
komi_by_score(struct uct_dynkomi *d, struct board *b, struct tree *tree, enum stone color)
{
	struct dynkomi_adaptive *a = d->data;
	if (d->score.playouts < TRUSTWORTHY_KOMI_PLAYOUTS)
		return tree->extra_komi;

	struct move_stats score = d->score;
	/* Almost-reset tree->score to gather fresh stats. */
	d->score.playouts = 1;

	/* Look at average score and push extra_komi in that direction. */
	float p = a->adapter(d, b);
	p = a->adapt_base + p * (1 - a->adapt_base);
	if (p > 0.9) p = 0.9; // don't get too eager!
	float extra_komi = tree->extra_komi + p * score.value;
	if (DEBUGL(3))
		fprintf(stderr, "mC += %f * %f\n", p, score.value);
	return extra_komi;
}

static float
komi_by_value(struct uct_dynkomi *d, struct board *b, struct tree *tree, enum stone color)
{
	struct dynkomi_adaptive *a = d->data;
	if (d->value.playouts < TRUSTWORTHY_KOMI_PLAYOUTS)
		return tree->extra_komi;

	struct move_stats value = d->value;
	/* Almost-reset tree->value to gather fresh stats. */
	d->value.playouts = 1;
	/* Correct color POV. */
	if (color == S_WHITE)
		value.value = 1 - value.value;

	/* We have three "value zones":
	 * red zone | yellow zone | green zone
	 *        ~45%           ~60%
	 * red zone: reduce komi
	 * yellow zone: do not touch komi
	 * green zone: enlage komi.
	 *
	 * Also, at some point komi will be tuned in such way
	 * that it will be in green zone but increasing it will
	 * be unfeasible. Thus, we have a _ratchet_ - we will
	 * remember the last komi that has put us into the
	 * red zone, and not use it or go over it. We use the
	 * ratchet only when giving extra komi, we always want
	 * to try to reduce extra komi we take.
	 *
	 * TODO: Make the ratchet expire after a while. */
	/* We use komi_by_color() first to normalize komi
	 * additions/subtractions, then apply it again on
	 * return value to restore original komi parity. */
	float extra_komi = komi_by_color(tree->extra_komi, color);
	int score_step = a->score_step;

	if (a->score_step_byavg != 0) {
		struct move_stats score = d->score;
		/* Almost-reset tree->score to gather fresh stats. */
		d->score.playouts = 1;
		/* Correct color POV. */
		if (color == S_WHITE)
			score.value = - score.value;
		if (score.value >= 0)
			score_step = round(score.value * a->score_step_byavg);
	}

	if (value.value < a->zone_red) {
		/* Red zone. Take extra komi. */
		if (DEBUGL(3))
			fprintf(stderr, "[red] %f, -= %d | komi ratchet %f -> %f\n",
				value.value, score_step, a->komi_ratchet, extra_komi);
		if (extra_komi > 0) a->komi_ratchet = extra_komi;
		extra_komi -= score_step;
		return komi_by_color(extra_komi, color);

	} else if (value.value < a->zone_green) {
		/* Yellow zone, do nothing. */
		return komi_by_color(extra_komi, color);

	} else {
		/* Green zone. Give extra komi. */
		extra_komi += score_step;
		if (DEBUGL(3))
			fprintf(stderr, "[green] %f, += %d | komi ratchet %f age %d\n",
				value.value, score_step, a->komi_ratchet, a->komi_ratchet_age);
		if (a->komi_ratchet_maxage > 0 && a->komi_ratchet_age > a->komi_ratchet_maxage) {
			a->komi_ratchet = 1000;
			a->komi_ratchet_age = 0;
		}
		if (a->use_komi_ratchet && extra_komi >= a->komi_ratchet) {
			extra_komi = a->komi_ratchet - 1;
			a->komi_ratchet_age++;
		}
		return komi_by_color(extra_komi, color);
	}
}

static float
adaptive_permove(struct uct_dynkomi *d, struct board *b, struct tree *tree)
{
	struct dynkomi_adaptive *a = d->data;
	if (DEBUGL(3))
		fprintf(stderr, "m %d/%d ekomi %f permove %f/%d\n",
			b->moves, a->lead_moves, tree->extra_komi,
			d->score.value, d->score.playouts);
	if (b->moves <= a->lead_moves)
		return board_effective_handicap(b, 7 /* XXX */);

	enum stone color = stone_other(tree->root_color);
	/* Get lower bound on komi we take so that we don't underperform
	 * too much. */
	float min_komi = komi_by_color(- a->max_losing_komi, color);

	float komi = a->indicator(d, b, tree, color);
	if (DEBUGL(3))
		fprintf(stderr, "dynkomi: %f -> %f\n", tree->extra_komi, komi);
	return komi_by_color(komi - min_komi, color) > 0 ? komi : min_komi;
}

static float
adaptive_persim(struct uct_dynkomi *d, struct board *b, struct tree *tree, struct tree_node *node)
{
	return tree->extra_komi;
}

struct uct_dynkomi *
uct_dynkomi_init_adaptive(struct uct *u, char *arg, struct board *b)
{
	struct uct_dynkomi *d = calloc2(1, sizeof(*d));
	d->uct = u;
	d->permove = adaptive_permove;
	d->persim = adaptive_persim;
	d->done = generic_done;

	struct dynkomi_adaptive *a = calloc2(1, sizeof(*a));
	d->data = a;

	if (board_size(b) - 2 >= 19)
		a->lead_moves = 20;
	else
		a->lead_moves = 4; // XXX
	a->max_losing_komi = 10;
	a->indicator = komi_by_score;

	a->adapter = adapter_sigmoid;
	a->adapt_rate = 20;
	a->adapt_phase = 0.5;
	a->adapt_moves = 200;
	a->adapt_dir = -0.5;

	a->zone_red = 0.45;
	a->zone_green = 0.6;
	a->score_step = 2;
	a->use_komi_ratchet = true;
	a->komi_ratchet_maxage = 0;
	a->komi_ratchet = 1000;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "lead_moves") && optval) {
				/* Do not adjust komi adaptively for first
				 * N moves. */
				a->lead_moves = atoi(optval);
			} else if (!strcasecmp(optname, "max_losing_komi") && optval) {
				a->max_losing_komi = atof(optval);
			} else if (!strcasecmp(optname, "indicator")) {
				/* Adaptatation indicator - how to decide
				 * the adaptation rate and direction. */
				if (!strcasecmp(optval, "value")) {
					/* Winrate w/ komi so far. */
					a->indicator = komi_by_value;
				} else if (!strcasecmp(optval, "score")) {
					/* Expected score w/ current komi. */
					a->indicator = komi_by_score;
				} else {
					fprintf(stderr, "UCT: Invalid indicator %s\n", optval);
					exit(1);
				}

				/* value indicator settings */
			} else if (!strcasecmp(optname, "zone_red") && optval) {
				a->zone_red = atof(optval);
			} else if (!strcasecmp(optname, "zone_green") && optval) {
				a->zone_green = atof(optval);
			} else if (!strcasecmp(optname, "score_step") && optval) {
				a->score_step = atoi(optval);
			} else if (!strcasecmp(optname, "score_step_byavg") && optval) {
				a->score_step_byavg = atof(optval);
			} else if (!strcasecmp(optname, "use_komi_ratchet")) {
				a->use_komi_ratchet = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "komi_ratchet_age") && optval) {
				a->komi_ratchet_maxage = atoi(optval);

				/* score indicator settings */
			} else if (!strcasecmp(optname, "adapter") && optval) {
				/* Adaptatation method. */
				if (!strcasecmp(optval, "sigmoid")) {
					a->adapter = adapter_sigmoid;
				} else if (!strcasecmp(optval, "linear")) {
					a->adapter = adapter_linear;
				} else {
					fprintf(stderr, "UCT: Invalid adapter %s\n", optval);
					exit(1);
				}
			} else if (!strcasecmp(optname, "adapt_base") && optval) {
				/* Adaptation base rate; see above. */
				a->adapt_base = atof(optval);
			} else if (!strcasecmp(optname, "adapt_rate") && optval) {
				/* Adaptation slope; see above. */
				a->adapt_rate = atof(optval);
			} else if (!strcasecmp(optname, "adapt_phase") && optval) {
				/* Adaptation phase shift; see above. */
				a->adapt_phase = atof(optval);
			} else if (!strcasecmp(optname, "adapt_moves") && optval) {
				/* Adaptation move amount; see above. */
				a->adapt_moves = atoi(optval);
			} else if (!strcasecmp(optname, "adapt_aport")) {
				a->adapt_aport = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "adapt_dir") && optval) {
				/* Adaptation direction vector; see above. */
				a->adapt_dir = atof(optval);

			} else {
				fprintf(stderr, "uct: Invalid dynkomi argument %s or missing value\n", optname);
				exit(1);
			}
		}
	}

	return d;
}
