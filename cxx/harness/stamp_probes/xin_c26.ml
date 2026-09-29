module Str = struct type t = string end
module Make (M : sig end) = struct include Str end
module A = struct include Make (struct end) end
