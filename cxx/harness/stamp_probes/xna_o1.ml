module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig type t end = struct module XSet = Set.Make( X ) type t =
    int end
end
