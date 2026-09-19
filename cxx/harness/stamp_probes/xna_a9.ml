module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( X ) end
  type t = M.XSet.t
end
