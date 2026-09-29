(* The result and the parameter out of the SAME unit: left alone. *)
module Mk (X : Map.OrderedType) : Map.S with type key = X.t =
  Map.Make (X)
