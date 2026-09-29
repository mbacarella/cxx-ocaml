

(* Test that we do not use local equation introduced by partial matching when computing
   the value kinds of function parameters *)

[@@@warning "-8"]

type 'a rep = Float : float rep | Int : int rep


(** Expected kind [int -> any -> float]:
    Since the match on [Float] can fail, one cannot use the equation [a = Float]
    to infer the kind [float] for [a].
*)
let curried : type a. a rep -> a -> float = fun Float a -> a +. 1.


(** Expected kind [any -> any -> float]:
    The [Type.Equal] pattern is total, however the default expression
    can raise and thus the pattern match in
    {[
      match assert false with
      | Type.Equal -> ...
    ]}
    fails to account for the exception case.
    Thus once again, we cannot use the equation [a = Float] to infer the kind
    [float] for [a].
*)
let opt (type a) ?p:(Type.Equal:(a,float) Type.eq=assert false) (x:a) = 1. +. x


(** Expected kind: [int -> float -> float]
    [Type.Equal] is an irrefutable pattern, we can safely use [a=float] to infer
    [a:float].
*)
let curried_ok : type a. (a,float) Type.eq -> a -> float = fun Type.Equal a -> a +. 1.



(** Expected kind: [any -> int -> int -> any -> any -> float],
    valid optimization [any -> int -> int -> float -> any -> float].
    The empty list pattern [[]] is an refutable pattern, we are currently refusing
    to use any GADTs equation after this pattern. However, the equation [a=float]
    is introduced by an irrefutable pattern without any dependencies on a refutable
    pattern. We could thus theoretically use this equation to refine the inferred
    kind for the function to [any -> int -> int -> float -> any].
    A good first approximation to implement this refinement would be to only freeze
    the set of GADTs equation when we observe a refutable pattern argument which
    adds a local equation.
*)
let curried_first : type a b. unit list -> (a,float) Type.eq -> b rep -> a -> b -> float =
  fun [] Type.Equal Float a b -> a +. b
