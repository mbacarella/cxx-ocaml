module P (B : Set.OrderedType) =
  Set.Make ((B : Set.OrderedType with type t = B.t))
