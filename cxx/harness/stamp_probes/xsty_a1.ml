(* an included type a later one shadows is dropped *)
module X = struct type t = int end
include X
type t = float
