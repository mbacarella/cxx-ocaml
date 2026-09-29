

(* Test that we do not use local equation introduced by partial matching when computing
   the value kinds of function parameters *)

[@@@warning "-8"]

type 'a rep = Float : float rep | Int : int rep


(** Expected kind [int -> any -> float]:
    Since the match on [Float] can fail, one cannot use the equation [a = Float]
    to infer the kind [float] for [a].
*)
let curried : type a. a rep -> a -> float = fun Float a -> a +. 1.
