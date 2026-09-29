module F (X : Set.OrderedType) : sig val x : int end = struct module S = Set.Make(X) let x = 0 end
