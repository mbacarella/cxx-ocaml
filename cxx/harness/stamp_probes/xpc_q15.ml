module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b (x : int M.s) = object end
