module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( X ) end
  type v = Set.Make( X ).t
  type u = Set.Make( Y ).t
end
