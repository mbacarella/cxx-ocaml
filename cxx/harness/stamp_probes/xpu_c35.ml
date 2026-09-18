module type E = sig end
module Ephemeron = struct module K1 = struct type t end end open Ephemeron
  let _ = (module K1 : E)
