module F ( X : Set.OrderedType ) ( Y : Set.OrderedType ) = struct
  module rec Mod : sig module XSet : sig type t end type u = XSet.t end = struct
    module XSet = Set.Make( X ) type u = XSet.t end
end
