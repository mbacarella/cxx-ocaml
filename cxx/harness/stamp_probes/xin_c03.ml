module A = struct module Str = struct type t end
module Make (M : sig end) = struct include Str end end
