module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end end =
    (struct module XSet = Set.Make( X ) end : sig module XSet : sig type t end
      end)
end
