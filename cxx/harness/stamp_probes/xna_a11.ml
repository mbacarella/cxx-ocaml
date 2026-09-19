module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end end = struct
    module XSet : sig type t end = Set.Make( X ) end
end
