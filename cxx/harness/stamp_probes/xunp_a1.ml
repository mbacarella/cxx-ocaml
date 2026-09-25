let f (m : (module Set.OrderedType with type t = int)) =
  let module M = (val m) in M.compare
