(* Tardygrada BFT Consensus — Safety Proof *)
(*
 * We prove: if fewer than N/2 replicas are corrupted,
 * majority vote returns the original value.
 *
 * This models the consensus protocol in src/vm/memory.c:
 * - N replicas, each holding a value
 * - Hash each replica's value
 * - Majority vote on hashes (> N/2 must agree)
 * - If majority matches birth_hash, return the value
 *)

Require Import Coq.Arith.Arith.
Require Import Coq.Lists.List.
Require Import Coq.Bool.Bool.
Require Import Lia.
Import ListNotations.

(* A replica is either honest (holds original value) or corrupted *)
Inductive Replica : Type :=
  | Honest : nat -> Replica
  | Corrupt : nat -> Replica.

(* Extract the value from a replica *)
Definition replica_value (r : Replica) : nat :=
  match r with
  | Honest v => v
  | Corrupt v => v
  end.

(* Count occurrences of a value in a list of replicas *)
Fixpoint count_value (v : nat) (replicas : list Replica) : nat :=
  match replicas with
  | [] => 0
  | r :: rest =>
    if Nat.eqb (replica_value r) v
    then S (count_value v rest)
    else count_value v rest
  end.

(* Count honest replicas *)
Fixpoint count_honest (replicas : list Replica) : nat :=
  match replicas with
  | [] => 0
  | Honest _ :: rest => S (count_honest rest)
  | Corrupt _ :: rest => count_honest rest
  end.

(* Count corrupt replicas *)
Fixpoint count_corrupt (replicas : list Replica) : nat :=
  match replicas with
  | [] => 0
  | Honest _ :: rest => count_corrupt rest
  | Corrupt _ :: rest => S (count_corrupt rest)
  end.

(* All honest replicas hold the same original value *)
Fixpoint all_honest_agree (v : nat) (replicas : list Replica) : Prop :=
  match replicas with
  | [] => True
  | Honest v' :: rest => v = v' /\ all_honest_agree v rest
  | Corrupt _ :: rest => all_honest_agree v rest
  end.

(* A value has majority if it appears in more than half the replicas *)
Definition has_majority (v : nat) (replicas : list Replica) : Prop :=
  2 * count_value v replicas > length replicas.

(* Honest + corrupt = total *)
Lemma honest_corrupt_total :
  forall replicas,
  count_honest replicas + count_corrupt replicas = length replicas.
Proof.
  induction replicas as [| r rest IH].
  - reflexivity.
  - destruct r; simpl; rewrite IH; reflexivity.
Qed.

(* Key lemma: honest replicas all vote for the original value *)
Lemma honest_votes_for_original :
  forall v replicas,
  all_honest_agree v replicas ->
  count_value v replicas >= count_honest replicas.
Proof.
  intros v replicas. induction replicas as [| r rest IH].
  - simpl. auto.
  - destruct r as [v' | v'].
    + simpl. intros [Heq Hrest].
      subst v'.
      rewrite Nat.eqb_refl.
      apply le_n_S. apply IH. exact Hrest.
    + simpl. intros Hrest.
      destruct (Nat.eqb (replica_value (Corrupt v')) v) eqn:E.
      * apply le_S. apply IH. exact Hrest.
      * apply IH. exact Hrest.
Qed.

(* ============================================
 * MAIN THEOREM: Safety of Byzantine majority vote
 *
 * If fewer than half the replicas are corrupt, and all honest
 * replicas agree on value v, then v has majority.
 *
 * This is THE guarantee that makes @hardened and @sovereign
 * immutability trustworthy. If this theorem holds, a corrupted
 * minority cannot change an agent's value.
 * ============================================ *)

Theorem bft_safety :
  forall v replicas,
  all_honest_agree v replicas ->
  2 * count_corrupt replicas < length replicas ->
  has_majority v replicas.
Proof.
  intros v replicas Hagree Hcorrupt.
  unfold has_majority.
  assert (Hhonest: count_value v replicas >= count_honest replicas)
    by (apply honest_votes_for_original; exact Hagree).
  assert (Htotal: count_honest replicas + count_corrupt replicas = length replicas)
    by (apply honest_corrupt_total).
  lia.
Qed.

(* Corollary: 3 replicas, at most 1 corrupt (@hardened default) *)
Corollary bft_3_replicas :
  forall v r1 r2 r3,
  all_honest_agree v [r1; r2; r3] ->
  count_corrupt [r1; r2; r3] <= 1 ->
  has_majority v [r1; r2; r3].
Proof.
  intros. apply bft_safety; auto. simpl. lia.
Qed.

(* Corollary: 5 replicas, at most 2 corrupt (@sovereign default) *)
Corollary bft_5_replicas :
  forall v r1 r2 r3 r4 r5,
  all_honest_agree v [r1; r2; r3; r4; r5] ->
  count_corrupt [r1; r2; r3; r4; r5] <= 2 ->
  has_majority v [r1; r2; r3; r4; r5].
Proof.
  intros. apply bft_safety; auto. simpl. lia.
Qed.

(* Uniqueness: at most one value can have majority *)
Theorem majority_unique :
  forall v1 v2 replicas,
  has_majority v1 replicas ->
  has_majority v2 replicas ->
  v1 <> v2 ->
  False.
Proof.
  intros v1 v2 replicas Hmaj1 Hmaj2 Hneq.
  unfold has_majority in *.
  (* Two different values can't both have > N/2 votes
     because that would require > N total votes *)
  assert (Hbound: count_value v1 replicas + count_value v2 replicas <= length replicas).
  { clear Hmaj1 Hmaj2.
    induction replicas as [| r rest IH].
    - simpl. lia.
    - simpl.
      destruct (Nat.eqb (replica_value r) v1) eqn:E1;
      destruct (Nat.eqb (replica_value r) v2) eqn:E2.
      + (* Both match — impossible since v1 <> v2 *)
        apply Nat.eqb_eq in E1. apply Nat.eqb_eq in E2.
        exfalso. apply Hneq. lia.
      + simpl. specialize (IH). lia.
      + simpl. specialize (IH). lia.
      + simpl. specialize (IH). lia.
  }
  lia.
Qed.
