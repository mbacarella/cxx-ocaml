(* companion for s638: `include M` where M holds BOTH a real submodule and a
   written alias to it.  The real one strengthens to a PRESENT alias; the
   written one stays ABSENT. *)
module M = struct
  module Inner = struct let v = 7 end
  module Ali = Inner
  module Other = struct let w = 9 end
end
include M
