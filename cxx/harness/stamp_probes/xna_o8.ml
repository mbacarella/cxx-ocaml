module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig module XSet : Set.S end = struct module XSet = Set.Make(
    X ) end
end
