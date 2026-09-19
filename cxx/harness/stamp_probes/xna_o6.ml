module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( X ) end
  and N : sig type t end = struct module YSet = Set.Make( Y ) type t = int end
end
