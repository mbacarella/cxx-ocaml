module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : Set.S with type t = Set.Make( X ).t end = struct
    module XSet = Set.Make( X ) end
end
