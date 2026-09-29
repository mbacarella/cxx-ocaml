

(* Test that we do not use local equation introduced by partial matching when computing
   the value kinds of function parameters *)

[@@@warning "-8"]

type 'a rep = Float : float rep | Int : int rep
