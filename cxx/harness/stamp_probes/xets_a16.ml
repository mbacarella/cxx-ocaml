let f1 ?x y = ignore x; ignore y
class c = object val v : int -> unit = f1 method m = v 1 end
