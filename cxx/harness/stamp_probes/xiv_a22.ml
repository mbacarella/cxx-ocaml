class a = object val x = 1 val y = 2 end
class b = object inherit a end
class c = object inherit b inherit a end
