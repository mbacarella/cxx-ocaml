module W = struct module F ( X : Set.OrderedType ) = struct module rec M : Set.S
  with type elt = X.t = Set.Make( X ) and P : sig type t end = struct type t =
  int end end end
