module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type elt = X.t end end =
    struct module XSet = Set.Make( X ) end
end
