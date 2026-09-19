module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end
