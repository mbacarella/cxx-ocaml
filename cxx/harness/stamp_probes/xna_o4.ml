module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig module XSet : sig type t end module YSet : sig type t end
    end =
    struct module XSet = Set.Make( X ) module YSet = Set.Make( Y ) end
end
