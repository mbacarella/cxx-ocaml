module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( X ) end
  type u = Set.Make( X ).t
end
