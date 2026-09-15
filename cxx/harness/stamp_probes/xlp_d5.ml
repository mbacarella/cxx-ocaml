(* The result ANOTHER UNIT's module type, the parameter another's again. *)
module Mk (X : Set.OrderedType) : Map.S with type key = X.t =
  Map.Make (X)
