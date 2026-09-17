module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include (Str : sig type t = string end) end
end
