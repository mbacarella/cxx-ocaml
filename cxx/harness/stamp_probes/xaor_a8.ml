let f (module M : Set.S with type elt = int) = M.empty
type t = (module Set.OrderedType)
