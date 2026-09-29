module F ( X : Set.OrderedType ) = struct
  module XSet : sig type t = Set.Make( X ).t end = Set.Make( X )
end
