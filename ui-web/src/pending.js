// The pending-diff card at the foot of the agent dock: what has been proposed,
// and the two things you can do about it.
//
// The model (pendingmodel.js) is where a proposal becomes text; this file only
// draws it. Same rules as every other renderer here (GUIDELINES 3): elements are
// built once and hidden rather than removed, every write is guarded by a cached
// value, and text goes through `.nodeValue`. There are eight nodes in this card
// and it still follows all of it — a rule that is skipped where it is cheap is a
// rule that is no longer true anywhere.
//
// Every guard compares the STRING it is about to write, not the proposal's `seq`.
// A re-proposal that changed only its reason line would leave any counter-shaped
// key standing still while the content moved, which is the one bug this project
// keeps having (GUIDELINES 2.1). Comparing the thing is also what is cheap here:
// the card holds four strings.

import { TITLE } from './pendingmodel.js';

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

function text(parent) {
  parent.appendChild(document.createTextNode(''));
  return parent.firstChild;
}

function button(cls, parent, label) {
  const el = document.createElement('button');
  el.className = cls;
  el.type = 'button';
  el.appendChild(document.createTextNode(label));
  parent.appendChild(el);
  return el;
}

export class Pending {
  /**
   * @param {HTMLElement} host
   * @param {{onApply?:function, onDiscard?:function}} handlers
   *
   * Neither handler sends anything. `onApply` is told that the user said yes;
   * the caller is the side that holds the socket and decides what a batch of ops
   * becomes on the wire.
   */
  constructor(host, { onApply, onDiscard } = {}) {
    this.host = host;
    this.host.className = 'pd';
    this.onApply = onApply;
    this.onDiscard = onDiscard;

    this.card = div('pd-card', host);
    const head = div('pd-head', this.card);
    // Written once: the card's name never changes, so it is not a per-draw
    // decision and does not need a guard.
    text(div('pd-title', head)).nodeValue = TITLE;
    this.meta = text(div('pd-meta', head));
    this.reason = text(div('pd-reason', this.card));

    this.acts = div('pd-acts', this.card);
    this.applyEl = button('pd-btn pd-apply', this.acts, 'Apply');
    this.applyText = this.applyEl.firstChild;
    this.discardEl = button('pd-btn pd-discard', this.acts, 'Discard');

    // `click`, not `pointerdown`: Apply commits a batch of edits, and a press
    // that the user drags off the button before releasing means they changed
    // their mind. It is also the event a keyboard sends, so the button works
    // without a pointer at all. Two listeners, bound once — nothing here is
    // pooled, so there is nothing to rebind.
    this.applyEl.addEventListener('click', () => this.onApply && this.onApply());
    this.discardEl.addEventListener('click', () => this.onDiscard && this.onDiscard());

    this.vm = null;
    this._meta = null; this._reason = null; this._apply = null;
    this._status = null; this._acts = null;
  }

  render(vm) {
    this.vm = vm;
    if (this._status !== vm.status) {
      this._status = vm.status;
      // The dashed accent edge means provisional; every other state is a record
      // of something already decided, so it goes quiet and solid.
      this.card.className = 'pd-card ' + vm.status;
      this.card.dataset.status = vm.status;
    }
    if (this._meta !== vm.meta) { this._meta = vm.meta; this.meta.nodeValue = vm.meta; }
    if (this._reason !== vm.reason) { this._reason = vm.reason; this.reason.nodeValue = vm.reason; }
    if (this._apply !== vm.applyLabel) { this._apply = vm.applyLabel; this.applyText.nodeValue = vm.applyLabel; }
    // Hidden rather than removed, and hidden rather than merely dimmed: a button
    // that is drawn but refuses is a control that ignores you, and there is
    // nothing to apply when nothing has been proposed.
    if (this._acts !== vm.actionable) {
      this._acts = vm.actionable;
      this.acts.style.display = vm.actionable ? '' : 'none';
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      status: vm.status,
      title: TITLE,
      label: vm.label,
      summary: vm.summary,
      meta: vm.meta,
      reason: vm.reason,
      ops: vm.opCount,
      // What the batch is, without the card having to draw it — an agent
      // asserting on a proposal wants the ops, not the sentence about them.
      types: vm.ops.map((o) => o.type),
      version: vm.version,
      applyLabel: vm.applyLabel,
      // The honesty flags, exposed the way the mixer exposed `authoritative`:
      // a test can hold this side to them, so they cannot be quietly dropped.
      previewed: vm.previewed,
      engineProposed: false,
      actionable: vm.actionable,
      seq: vm.seq,
      domNodes: this.host.querySelectorAll('*').length,
    };
  }
}
