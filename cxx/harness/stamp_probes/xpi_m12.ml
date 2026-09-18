module A = struct type t = int let compare = compare end
let f (type a) (module X : Set.OrderedType with type t = a) = ()
let _ = f (module A)
