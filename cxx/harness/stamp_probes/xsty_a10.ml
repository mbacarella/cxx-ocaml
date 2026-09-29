(* a second include shadows the first's type *)
include struct type t = int end
include struct type t = float end
module Y = struct type z end
