module Str = struct type t = string end
module Make (M : sig end) = struct include Str end
module B = Make (Str)
