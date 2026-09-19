module type ORDERED = sig type t end module type HEAP = sig module Elem: sig
  module Z : sig type t end end end
