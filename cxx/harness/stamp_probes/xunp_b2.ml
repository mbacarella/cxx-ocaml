let f m = let module M = (val m : Set.OrderedType with type t = int) in
  M.compare
