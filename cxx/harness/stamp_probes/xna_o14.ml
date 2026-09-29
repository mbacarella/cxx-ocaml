module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( Y ) end
  module N = Set.Make( X )
end
