module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str type v = t end end
