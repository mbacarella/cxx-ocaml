module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end
module Make2 (M : sig end) = struct include Str end end
