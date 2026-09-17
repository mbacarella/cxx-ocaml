module A = struct module Str = struct let x = 1 end
module Make (M : sig end) = struct include Str end end
