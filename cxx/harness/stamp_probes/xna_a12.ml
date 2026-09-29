module F ( X : Set.OrderedType ) = struct
  module M = struct
    module XSet : sig type t end = Set.Make( X ) end
end
