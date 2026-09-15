(* The result another unit's, the parameter WRITTEN OUT. *)
module Mk (X : sig type t end) : Map.S with type key = X.t =
  Map.Make (struct type t = X.t let compare = compare end)
