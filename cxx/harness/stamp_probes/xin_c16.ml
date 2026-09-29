module Str = struct type t = string end
module A = struct module Make (M : sig end) = struct type u = int include Str
end end
